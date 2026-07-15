const std = @import("std");
const builtin = @import("builtin");
const core_adapter = @import("core_adapter.zig");
const native_sdk = @import("native_sdk");
const runner = @import("runner");

pub const panic = std.debug.FullPanic(native_sdk.debug.capturePanic);

const canvas = native_sdk.canvas;
const geometry = native_sdk.geometry;

pub const canvas_label = "library-canvas";
pub const window_width: f32 = 960;
pub const window_height: f32 = 680;
pub const window_min_width: f32 = 560;
pub const window_min_height: f32 = 400;
const header_natural_height: f32 = 52;
const max_courses = 20;
const max_course_id_bytes = 128;
const max_course_name_bytes = 512;
const max_library_message_bytes = 256;

const shell_views = [_]native_sdk.ShellView{
    .{ .label = canvas_label, .kind = .gpu_surface, .fill = true, .role = "Library canvas", .accessibility_label = "melearner Library", .gpu_backend = .metal, .gpu_pixel_format = .bgra8_unorm, .gpu_present_mode = .timer, .gpu_alpha_mode = .@"opaque", .gpu_color_space = .srgb, .gpu_vsync = true },
};
const shell_windows = [_]native_sdk.ShellWindow{.{
    .label = "main",
    .title = "melearner",
    .width = window_width,
    .height = window_height,
    .min_width = window_min_width,
    .min_height = window_min_height,
    .restore_state = false,
    .views = &shell_views,
}};
const shell_scene: native_sdk.ShellConfig = .{ .windows = &shell_windows };

pub const Course = struct {
    id_storage: [max_course_id_bytes]u8 = [_]u8{0} ** max_course_id_bytes,
    id_len: usize = 0,
    name_storage: [max_course_name_bytes]u8 = [_]u8{0} ** max_course_name_bytes,
    name_len: usize = 0,
    lesson_count: u64 = 0,
    completed_lesson_count: u64 = 0,
    progress_percent: u32 = 0,

    pub fn id(course: *const Course) []const u8 {
        return course.id_storage[0..course.id_len];
    }

    pub fn name(course: *const Course) []const u8 {
        return course.name_storage[0..course.name_len];
    }

    pub fn progressLine(course: *const Course, arena: std.mem.Allocator) []const u8 {
        return std.fmt.allocPrint(arena, "{d} of {d} Lessons \u{b7} {d}%", .{
            course.completed_lesson_count,
            course.lesson_count,
            course.progress_percent,
        }) catch "";
    }
};

pub const LibraryState = enum {
    opening,
    empty,
    ready,
    failed,
};

pub const Model = struct {
    chrome_leading: f32 = 0,
    header_height: f32 = header_natural_height,
    library_state: LibraryState = .opening,
    library_revision: u64 = 0,
    total_courses: u64 = 0,
    courses: [max_courses]Course = undefined,
    course_count: usize = 0,
    library_message_storage: [max_library_message_bytes]u8 = [_]u8{0} ** max_library_message_bytes,
    library_message_len: usize = 0,

    pub const view_unbound = .{
        "library_state",
        "library_revision",
        "total_courses",
        "courses",
        "course_count",
        "library_message_storage",
        "library_message_len",
        "libraryReady",
    };

    pub fn libraryOpening(model: *const Model) bool {
        return model.library_state == .opening;
    }

    pub fn libraryEmpty(model: *const Model) bool {
        return model.library_state == .empty;
    }

    pub fn libraryReady(model: *const Model) bool {
        return model.library_state == .ready;
    }

    pub fn libraryFailed(model: *const Model) bool {
        return model.library_state == .failed;
    }

    pub fn libraryMessage(model: *const Model) []const u8 {
        return model.library_message_storage[0..model.library_message_len];
    }

    pub fn courseRows(model: *const Model, arena: std.mem.Allocator) []const Course {
        _ = arena;
        return model.courses[0..model.course_count];
    }

    pub fn courseTotalLabel(model: *const Model, arena: std.mem.Allocator) []const u8 {
        if (model.course_count < model.total_courses) {
            return std.fmt.allocPrint(arena, "{d} of {d} Courses", .{
                model.course_count,
                model.total_courses,
            }) catch "";
        }
        return std.fmt.allocPrint(arena, "{d} {s}", .{
            model.total_courses,
            if (model.total_courses == 1) "Course" else "Courses",
        }) catch "";
    }

    fn setFailure(model: *Model, message: []const u8) void {
        const value = if (message.len == 0 or message.len > max_library_message_bytes or !std.unicode.utf8ValidateSlice(message))
            "The Library service could not open."
        else
            message;
        @memcpy(model.library_message_storage[0..value.len], value);
        model.library_message_len = value.len;
        model.library_state = .failed;
        model.library_revision = 0;
        model.total_courses = 0;
        model.course_count = 0;
    }

    fn loadCoursePage(model: *Model, bytes: []const u8) !void {
        const CoursePayload = struct {
            id: []const u8,
            name: []const u8,
            lessonCount: u64,
            completedLessonCount: u64,
            progressPercent: u32,
        };
        const PagePayload = struct {
            revision: u64,
            offset: u64,
            total: u64,
            rows: []const CoursePayload,
        };

        const parsed = try std.json.parseFromSlice(PagePayload, std.heap.page_allocator, bytes, .{
            .ignore_unknown_fields = true,
        });
        defer parsed.deinit();
        const page = parsed.value;
        if (page.revision == 0 or page.offset != 0 or page.rows.len > max_courses or page.total < page.rows.len) {
            return error.InvalidLibraryPage;
        }
        if ((page.total == 0) != (page.rows.len == 0)) return error.InvalidLibraryPage;

        var courses: [max_courses]Course = undefined;
        for (page.rows, 0..) |source, index| {
            if (source.id.len == 0 or source.id.len > max_course_id_bytes or
                source.name.len == 0 or source.name.len > max_course_name_bytes or
                source.completedLessonCount > source.lessonCount or source.progressPercent > 100)
            {
                return error.InvalidLibraryPage;
            }
            for (courses[0..index]) |course| {
                if (std.mem.eql(u8, course.id(), source.id)) return error.InvalidLibraryPage;
            }
            courses[index] = .{
                .id_len = source.id.len,
                .name_len = source.name.len,
                .lesson_count = source.lessonCount,
                .completed_lesson_count = source.completedLessonCount,
                .progress_percent = source.progressPercent,
            };
            @memcpy(courses[index].id_storage[0..source.id.len], source.id);
            @memcpy(courses[index].name_storage[0..source.name.len], source.name);
        }

        @memcpy(model.courses[0..page.rows.len], courses[0..page.rows.len]);
        model.course_count = page.rows.len;
        model.library_revision = page.revision;
        model.total_courses = page.total;
        model.library_message_len = 0;
        model.library_state = if (page.total == 0) .empty else .ready;
    }
};

pub const Msg = union(enum) {
    chrome_changed: native_sdk.WindowChrome,
    library_loaded: native_sdk.EffectExternalResult,

    pub const view_unbound = .{ "chrome_changed", "library_loaded" };
};

pub const LibraryUi = canvas.Ui(Msg);
pub const library_markup = @embedFile("library.native");
pub const CompiledLibraryView = canvas.CompiledMarkupView(Model, Msg, library_markup);

const dev_markup_reload = builtin.mode == .Debug;
pub const LibraryApp = native_sdk.UiAppWithFeatures(Model, Msg, .{ .runtime_markup = dev_markup_reload });
pub const Effects = LibraryApp.Effects;

pub fn boot(model: *Model, effects: *Effects) void {
    model.library_state = .opening;
    _ = effects.external(.{
        .key = core_adapter.request_key,
        .adapter_id = core_adapter.adapter_id,
        .kind = @intFromEnum(core_adapter.Operation.open_library),
        .schema_version = core_adapter.schema_version,
        .payload = "",
        .on_result = Effects.externalMsg(.library_loaded),
    }) catch model.setFailure("The Library service could not start.");
}

pub fn update(model: *Model, msg: Msg, effects: *Effects) void {
    _ = effects;
    switch (msg) {
        .chrome_changed => |chrome| {
            model.chrome_leading = chrome.insets.left;
            model.header_height = @max(header_natural_height, chrome.insets.top);
        },
        .library_loaded => |result| {
            if (result.key != core_adapter.request_key or
                result.adapter_id != core_adapter.adapter_id or
                result.kind != @intFromEnum(core_adapter.Operation.open_library) or
                result.schema_version != core_adapter.schema_version)
            {
                model.setFailure("The Library service returned an unexpected response.");
                return;
            }
            switch (result.outcome) {
                .ok => model.loadCoursePage(result.bytes) catch
                    model.setFailure("The Library service returned invalid data."),
                .failed => model.setFailure(result.bytes),
                .cancelled => model.setFailure("Opening the Library was cancelled."),
                .adapter_unavailable, .submit_failed => model.setFailure("The Library service is unavailable."),
            }
        },
    }
}

pub fn onChrome(chrome: native_sdk.WindowChrome) ?Msg {
    return .{ .chrome_changed = chrome };
}

pub fn main(init: std.process.Init) !void {
    var state_dir_buffer: [std.Io.Dir.max_path_bytes]u8 = undefined;
    const state_dir = try native_sdk.app_dirs.resolveOne(
        .{ .name = "melearner" },
        native_sdk.app_dirs.currentPlatform(),
        native_sdk.debug.envFromMap(init.environ_map),
        .data,
        &state_dir_buffer,
    );
    const adapter = try core_adapter.CoreAdapter.create(std.heap.page_allocator, init.io, state_dir);
    defer adapter.destroy();

    const app_state = try std.heap.page_allocator.create(LibraryApp);
    defer std.heap.page_allocator.destroy(app_state);
    app_state.* = LibraryApp.init(std.heap.page_allocator, .{}, .{
        .name = "melearner",
        .scene = shell_scene,
        .canvas_label = canvas_label,
        .update_fx = update,
        .init_fx = boot,
        .on_chrome = onChrome,
        .view = CompiledLibraryView.build,
        .markup = if (dev_markup_reload)
            .{ .source = library_markup, .watch_path = "src/library.native", .io = init.io }
        else
            null,
    });
    defer app_state.deinit();
    app_state.effects.bindExternalAdapter(adapter.binding());

    try runner.runWithOptions(app_state.app(), .{
        .app_name = "melearner",
        .window_title = "melearner",
        .bundle_id = "com.whitehades.melearner",
        .default_frame = geometry.RectF.init(0, 0, window_width, window_height),
        .restore_state = false,
        .js_window_api = false,
    }, init);
}

test {
    _ = @import("tests.zig");
}
