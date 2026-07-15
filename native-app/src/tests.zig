const std = @import("std");
const native_sdk = @import("native_sdk");
const core_adapter = @import("core_adapter.zig");
const main = @import("main.zig");

const canvas = native_sdk.canvas;
const geometry = native_sdk.geometry;
const testing = std.testing;

const LibraryMarkup = canvas.MarkupView(main.Model, main.Msg);

fn buildTree(arena: std.mem.Allocator, model: *const main.Model) !main.LibraryUi.Tree {
    var ui = main.LibraryUi.init(arena);
    return ui.finalizeWithTokens(main.rootView(&ui, model), main.tokensFromModel(model));
}

fn buildMarkupTree(arena: std.mem.Allocator, model: *const main.Model) !main.LibraryUi.Tree {
    var view = try LibraryMarkup.init(arena, main.library_markup);
    var ui = main.LibraryUi.init(arena);
    return ui.finalizeWithTokens(try view.build(&ui, model), main.tokensFromModel(model));
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

fn setCourse(course: *main.Course, id: []const u8, name: []const u8, available: bool) void {
    course.* = .{
        .id_len = id.len,
        .name_len = name.len,
        .lesson_count = 10,
        .completed_lesson_count = 4,
        .progress_percent = 40,
        .available = available,
    };
    @memcpy(course.id_storage[0..id.len], id);
    @memcpy(course.name_storage[0..name.len], name);
}

fn populatedLibraryModel() main.Model {
    var model = main.Model{
        .library_state = .ready,
        .library_revision = 7,
        .total_courses = 21,
        .course_count = 2,
    };
    setCourse(&model.courses[0], "course-1", "Systems", true);
    setCourse(&model.courses[1], "course-2", "Archived Systems", false);
    return model;
}

fn largeCourseModel() main.Model {
    var model = main.Model{
        .library_revision = 8,
        .screen = .course,
        .course_state = .ready,
        .total_lessons = 100_000,
        .lesson_count = core_adapter.lesson_page_size,
    };
    selectCourse(&model, "course-1", "Systems");
    for (model.lessons[0..model.lesson_count], 0..) |*lesson, index| {
        var id_buffer: [32]u8 = undefined;
        var name_buffer: [32]u8 = undefined;
        const id = std.fmt.bufPrint(&id_buffer, "lesson-{d}", .{index + 1}) catch unreachable;
        const name = std.fmt.bufPrint(&name_buffer, "Lesson {d}", .{index + 1}) catch unreachable;
        lesson.* = .{
            .id_len = id.len,
            .section_id_len = "section-1".len,
            .section_name_len = "Foundations".len,
            .name_len = name.len,
            .kind_len = "video".len,
            .duration = 120,
            .starts_section = index == 0,
        };
        @memcpy(lesson.id_storage[0..id.len], id);
        @memcpy(lesson.section_id_storage[0.."section-1".len], "section-1");
        @memcpy(lesson.section_name_storage[0.."Foundations".len], "Foundations");
        @memcpy(lesson.name_storage[0..name.len], name);
        @memcpy(lesson.kind_storage[0.."video".len], "video");
    }
    return model;
}

const LiveLibrary = struct {
    harness: *native_sdk.TestHarness(),
    app_state: *main.LibraryApp,
    app: native_sdk.App,

    fn start(model: main.Model, size: geometry.SizeF, appearance: native_sdk.Appearance) !LiveLibrary {
        const harness = try native_sdk.TestHarness().create(testing.allocator, .{ .size = size });
        errdefer harness.destroy(testing.allocator);
        harness.null_platform.gpu_surfaces = true;

        const app_state = try testing.allocator.create(main.LibraryApp);
        errdefer testing.allocator.destroy(app_state);
        var options = main.appOptions();
        options.init_fx = null;
        app_state.* = main.LibraryApp.init(testing.allocator, model, options);
        app_state.effects.executor = .fake;
        const app = app_state.app();
        try harness.start(app);
        try harness.runtime.dispatchPlatformEvent(app, .{ .appearance_changed = appearance });
        try harness.runtime.dispatchPlatformEvent(app, .{ .gpu_surface_frame = .{
            .label = main.canvas_label,
            .size = size,
            .scale_factor = 1,
            .frame_index = 1,
            .timestamp_ns = 1_000_000,
            .nonblank = true,
        } });
        return .{ .harness = harness, .app_state = app_state, .app = app };
    }

    fn stop(live: LiveLibrary) void {
        live.app_state.deinit();
        testing.allocator.destroy(live.app_state);
        live.harness.destroy(testing.allocator);
    }
};

fn snapshotByName(snapshot: native_sdk.automation.snapshot.Input, name: []const u8) ?native_sdk.automation.snapshot.Widget {
    for (snapshot.widgets) |widget| {
        if (std.mem.eql(u8, widget.name, name)) return widget;
    }
    return null;
}

fn snapshotByNameAndRole(snapshot: native_sdk.automation.snapshot.Input, name: []const u8, role: []const u8) ?native_sdk.automation.snapshot.Widget {
    for (snapshot.widgets) |widget| {
        if (std.mem.eql(u8, widget.name, name) and std.mem.eql(u8, widget.role, role)) return widget;
    }
    return null;
}

fn screenshotHash(runtime: anytype) !u64 {
    const pixel_size = try runtime.canvasScreenshotPixelSize(1, main.canvas_label, 1);
    const pixels = try testing.allocator.alloc(u8, pixel_size.byte_len);
    defer testing.allocator.free(pixels);
    const scratch = try testing.allocator.alloc(u8, pixel_size.byte_len);
    defer testing.allocator.free(scratch);
    const screenshot = try runtime.renderCanvasScreenshot(1, main.canvas_label, 1, pixels, scratch);

    return std.hash.Wyhash.hash(0, screenshot.rgba8);
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
    expected.metrics.control_height_sm = 40;
    expected.metrics.control_height = 40;
    expected.metrics.control_height_lg = 48;
    expected.metrics.row_extent = 40;
    try testing.expectEqualDeep(expected, main.tokensFromModel(&model));
}

test "the native theme follows the warm-paper and graphite token contract" {
    const light = main.tokensFromModel(&main.Model{});
    try testing.expectEqualDeep(canvas.Color.rgb8(245, 244, 237), light.colors.background);
    try testing.expectEqualDeep(canvas.Color.rgb8(250, 249, 245), light.colors.surface);
    try testing.expectEqualDeep(canvas.Color.rgb8(232, 230, 220), light.colors.surface_subtle);
    try testing.expectEqualDeep(canvas.Color.rgb8(228, 236, 245), light.colors.surface_pressed);
    try testing.expectEqualDeep(canvas.Color.rgb8(20, 20, 19), light.colors.text);
    try testing.expectEqualDeep(canvas.Color.rgb8(100, 97, 89), light.colors.text_muted);
    try testing.expectEqualDeep(canvas.Color.rgb8(227, 224, 211), light.colors.border);
    try testing.expectEqualDeep(canvas.Color.rgb8(27, 54, 93), light.colors.accent);
    try testing.expectEqualDeep(canvas.Color.rgb8(250, 249, 245), light.colors.accent_text);
    try testing.expectEqualDeep(canvas.Color.rgb8(27, 54, 93), light.colors.focus_ring);
    try testing.expectEqualDeep(canvas.Color.rgb8(238, 236, 227), light.colors.disabled);
    try testing.expectEqual(@as(f32, 40), light.metrics.control_height_sm);
    try testing.expectEqual(@as(f32, 40), light.metrics.control_height);
    try testing.expectEqual(@as(f32, 48), light.metrics.control_height_lg);
    try testing.expectEqual(@as(f32, 40), light.metrics.row_extent);
    try testing.expectEqual(@as(u32, 150), light.motion.fast_ms);
    try testing.expectEqual(@as(u32, 200), light.motion.normal_ms);
    try testing.expectEqual(@as(u32, 250), light.motion.slow_ms);

    const dark = main.tokensFromModel(&main.Model{ .appearance = .{ .color_scheme = .dark } });
    try testing.expectEqualDeep(canvas.Color.rgb8(16, 17, 19), dark.colors.background);
    try testing.expectEqualDeep(canvas.Color.rgb8(23, 23, 25), dark.colors.surface);
    try testing.expectEqualDeep(canvas.Color.rgb8(36, 35, 33), dark.colors.surface_subtle);
    try testing.expectEqualDeep(canvas.Color.rgb8(38, 52, 71), dark.colors.surface_pressed);
    try testing.expectEqualDeep(canvas.Color.rgb8(235, 232, 223), dark.colors.text);
    try testing.expectEqualDeep(canvas.Color.rgb8(166, 160, 149), dark.colors.text_muted);
    try testing.expectEqualDeep(canvas.Color.rgb8(45, 44, 41), dark.colors.border);
    try testing.expectEqualDeep(canvas.Color.rgb8(158, 184, 220), dark.colors.accent);
    try testing.expectEqualDeep(canvas.Color.rgb8(16, 17, 19), dark.colors.accent_text);
    try testing.expectEqualDeep(canvas.Color.rgb8(158, 184, 220), dark.colors.focus_ring);
    try testing.expectEqualDeep(canvas.Color.rgb8(34, 34, 34), dark.colors.disabled);
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
        .pending_request_id = 1,
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
        .pending_request_id = 1,
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

test "a stale Library request result is discarded while the current request remains active" {
    var effects = main.Effects.init(testing.allocator);
    defer effects.deinit();
    effects.executor = .fake;

    var model = main.Model{};
    main.boot(&model, &effects);
    const request = effects.pendingExternalAt(0).?;
    main.update(&model, .{ .library_loaded = .{
        .request_id = request.request_id + 1,
        .key = core_adapter.library_page_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.load_library_page),
        .schema_version = core_adapter.schema_version,
        .outcome = .ok,
        .bytes =
        \\{"revision":7,"offset":20,"total":21,"rows":[{"id":"course-21","name":"Course 21","missingSince":null,"lessonCount":1,"completedLessonCount":0,"progressPercent":0}]}
        ,
    } }, &effects);

    try testing.expect(model.libraryOpening());
    try testing.expectEqual(@as(u64, 0), model.pending_page_offset);
    try testing.expectEqual(request.request_id, model.pending_request_id);
    try testing.expectEqual(@as(usize, 0), model.course_count);
    try testing.expectEqualStrings("", model.libraryMessage());
}

test "a malformed current Library page remains an honest error" {
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
    model.pending_request_id = 1;
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

test "the populated native shell stays clean and caps its study surface on ultrawide windows" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();

    const model = populatedLibraryModel();
    const tree = try buildTree(arena_state.allocator(), &model);
    try canvas.expectLayoutAuditSweepClean(testing.allocator, tree.root, .{
        .tokens = main.tokensFromModel(&model),
        .min_size = native_sdk.geometry.SizeF.init(main.window_min_width, main.window_min_height),
        .default_size = native_sdk.geometry.SizeF.init(main.window_width, main.window_height),
        .large_size = native_sdk.geometry.SizeF.init(1920, 1080),
    });
    try canvas.expectA11yAuditSweepClean(testing.allocator, tree.root, .{
        .tokens = main.tokensFromModel(&model),
        .min_size = native_sdk.geometry.SizeF.init(main.window_min_width, main.window_min_height),
        .default_size = native_sdk.geometry.SizeF.init(main.window_width, main.window_height),
        .large_size = native_sdk.geometry.SizeF.init(1920, 1080),
    });
}

test "a 100000-Lesson Course mounts only its bounded page at every target size" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();

    const model = largeCourseModel();
    const tree = try buildTree(arena_state.allocator(), &model);
    try testing.expect(findByText(tree.root, .text, "1–20 of 100000 Lessons") != null);
    try testing.expect(findByText(tree.root, .text, "Lesson 20") != null);
    try testing.expect(findByText(tree.root, .text, "Lesson 21") == null);
    try canvas.expectLayoutAuditSweepClean(testing.allocator, tree.root, .{
        .tokens = main.tokensFromModel(&model),
        .min_size = geometry.SizeF.init(560, 400),
        .default_size = geometry.SizeF.init(960, 680),
        .large_size = geometry.SizeF.init(1920, 1080),
    });
}

test "keyboard focus opens the first available Course" {
    const live = try LiveLibrary.start(populatedLibraryModel(), geometry.SizeF.init(960, 680), .{});
    defer live.stop();

    try live.harness.runtime.dispatchAutomationCommand(live.app, "widget-key " ++ main.canvas_label ++ " tab");
    var snapshot = live.harness.runtime.automationSnapshot("melearner");
    try testing.expect((snapshotByName(snapshot, "Courses") orelse return error.TestUnexpectedResult).focused);
    try live.harness.runtime.dispatchAutomationCommand(live.app, "widget-key " ++ main.canvas_label ++ " tab");
    snapshot = live.harness.runtime.automationSnapshot("melearner");
    const course = snapshotByNameAndRole(snapshot, "Systems", "listitem") orelse return error.TestUnexpectedResult;
    try testing.expect(course.focused);
    try testing.expectEqualStrings("listitem", course.role);
    try testing.expect(course.actions.press);

    try live.harness.runtime.dispatchAutomationCommand(live.app, "widget-key " ++ main.canvas_label ++ " enter");
    try testing.expectEqual(main.Screen.course, live.app_state.model.screen);
    try testing.expectEqual(main.CourseState.accessing, live.app_state.model.course_state);
    try testing.expect(live.app_state.effects.pendingExternalAt(0) != null);
}

test "light and dark Library screenshots and semantic snapshots stay deterministic" {
    const cases = [_]struct {
        size: geometry.SizeF,
        expected_x: f32,
    }{
        .{ .size = geometry.SizeF.init(560, 400), .expected_x = 0 },
        .{ .size = geometry.SizeF.init(960, 680), .expected_x = 0 },
        .{ .size = geometry.SizeF.init(1920, 1080), .expected_x = 480 },
    };
    var screenshot_hashes: [cases.len * 2]u64 = undefined;

    for (cases, 0..) |case, index| {
        for ([_]native_sdk.Appearance{
            .{},
            .{ .color_scheme = .dark },
        }, 0..) |appearance, scheme_index| {
            const live = try LiveLibrary.start(populatedLibraryModel(), case.size, appearance);
            defer live.stop();
            const snapshot = live.harness.runtime.automationSnapshot("melearner");
            const library = snapshotByName(snapshot, "Library Courses") orelse return error.TestUnexpectedResult;
            try testing.expectEqual(case.expected_x, library.bounds.x);
            try testing.expectEqual(@min(case.size.width, main.content_max_width), library.bounds.width);
            try testing.expect(snapshotByNameAndRole(snapshot, "Systems", "listitem") != null);
            try testing.expectEqualStrings("progressbar", (snapshotByName(snapshot, "Systems Progress") orelse return error.TestUnexpectedResult).role);
            try testing.expect(snapshotByName(snapshot, "Archived Systems") != null);

            const result_index = index * 2 + scheme_index;
            screenshot_hashes[result_index] = try screenshotHash(&live.harness.runtime);
        }
    }

    try testing.expectEqualSlices(u64, &[_]u64{
        6559461630511852737,
        1307133522201074578,
        2879665690939281361,
        12495037189123983726,
        11865650813177772464,
        3164154168597024543,
    }, &screenshot_hashes);
}

test "compiled and interpreted Library views have the same widget identities" {
    var arena_state = std.heap.ArenaAllocator.init(testing.allocator);
    defer arena_state.deinit();
    const arena = arena_state.allocator();

    const model = main.Model{};
    const interpreted = try buildMarkupTree(arena, &model);
    var compiled_ui = main.LibraryUi.init(arena);
    const compiled = try compiled_ui.finalizeWithTokens(main.CompiledLibraryView.build(&compiled_ui, &model), main.tokensFromModel(&model));

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
